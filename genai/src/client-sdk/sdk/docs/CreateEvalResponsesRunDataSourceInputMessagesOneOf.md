# CreateEvalResponsesRunDataSourceInputMessagesOneOf


## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**type** | **str** | The type of input messages. Always &#x60;template&#x60;. | 
**template** | [**List[CreateEvalResponsesRunDataSourceInputMessagesOneOfTemplateInner]**](CreateEvalResponsesRunDataSourceInputMessagesOneOfTemplateInner.md) | A list of chat messages forming the prompt or context. May include variable references to the \&quot;item\&quot; namespace, ie {{item.name}}. | 

## Example

```python
from openapi_client.models.create_eval_responses_run_data_source_input_messages_one_of import CreateEvalResponsesRunDataSourceInputMessagesOneOf

# TODO update the JSON string below
json = "{}"
# create an instance of CreateEvalResponsesRunDataSourceInputMessagesOneOf from a JSON string
create_eval_responses_run_data_source_input_messages_one_of_instance = CreateEvalResponsesRunDataSourceInputMessagesOneOf.from_json(json)
# print the JSON string representation of the object
print(CreateEvalResponsesRunDataSourceInputMessagesOneOf.to_json())

# convert the object into a dict
create_eval_responses_run_data_source_input_messages_one_of_dict = create_eval_responses_run_data_source_input_messages_one_of_instance.to_dict()
# create an instance of CreateEvalResponsesRunDataSourceInputMessagesOneOf from a dict
create_eval_responses_run_data_source_input_messages_one_of_from_dict = CreateEvalResponsesRunDataSourceInputMessagesOneOf.from_dict(create_eval_responses_run_data_source_input_messages_one_of_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


