# CreateEvalResponsesRunDataSourceInputMessages


## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**type** | **str** | The type of input messages. Always &#x60;template&#x60;. | 
**template** | [**List[CreateEvalResponsesRunDataSourceInputMessagesOneOfTemplateInner]**](CreateEvalResponsesRunDataSourceInputMessagesOneOfTemplateInner.md) | A list of chat messages forming the prompt or context. May include variable references to the \&quot;item\&quot; namespace, ie {{item.name}}. | 
**item_reference** | **str** | A reference to a variable in the \&quot;item\&quot; namespace. Ie, \&quot;item.name\&quot; | 

## Example

```python
from openapi_client.models.create_eval_responses_run_data_source_input_messages import CreateEvalResponsesRunDataSourceInputMessages

# TODO update the JSON string below
json = "{}"
# create an instance of CreateEvalResponsesRunDataSourceInputMessages from a JSON string
create_eval_responses_run_data_source_input_messages_instance = CreateEvalResponsesRunDataSourceInputMessages.from_json(json)
# print the JSON string representation of the object
print(CreateEvalResponsesRunDataSourceInputMessages.to_json())

# convert the object into a dict
create_eval_responses_run_data_source_input_messages_dict = create_eval_responses_run_data_source_input_messages_instance.to_dict()
# create an instance of CreateEvalResponsesRunDataSourceInputMessages from a dict
create_eval_responses_run_data_source_input_messages_from_dict = CreateEvalResponsesRunDataSourceInputMessages.from_dict(create_eval_responses_run_data_source_input_messages_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


