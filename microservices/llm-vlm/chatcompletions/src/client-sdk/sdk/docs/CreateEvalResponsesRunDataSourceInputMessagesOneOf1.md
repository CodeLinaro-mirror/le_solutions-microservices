# CreateEvalResponsesRunDataSourceInputMessagesOneOf1


## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**type** | **str** | The type of input messages. Always &#x60;item_reference&#x60;. | 
**item_reference** | **str** | A reference to a variable in the \&quot;item\&quot; namespace. Ie, \&quot;item.name\&quot; | 

## Example

```python
from openapi_client.models.create_eval_responses_run_data_source_input_messages_one_of1 import CreateEvalResponsesRunDataSourceInputMessagesOneOf1

# TODO update the JSON string below
json = "{}"
# create an instance of CreateEvalResponsesRunDataSourceInputMessagesOneOf1 from a JSON string
create_eval_responses_run_data_source_input_messages_one_of1_instance = CreateEvalResponsesRunDataSourceInputMessagesOneOf1.from_json(json)
# print the JSON string representation of the object
print(CreateEvalResponsesRunDataSourceInputMessagesOneOf1.to_json())

# convert the object into a dict
create_eval_responses_run_data_source_input_messages_one_of1_dict = create_eval_responses_run_data_source_input_messages_one_of1_instance.to_dict()
# create an instance of CreateEvalResponsesRunDataSourceInputMessagesOneOf1 from a dict
create_eval_responses_run_data_source_input_messages_one_of1_from_dict = CreateEvalResponsesRunDataSourceInputMessagesOneOf1.from_dict(create_eval_responses_run_data_source_input_messages_one_of1_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


